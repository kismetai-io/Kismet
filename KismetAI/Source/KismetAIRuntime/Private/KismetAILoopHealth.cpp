// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAILoopHealth.h"
#include "KismetAIToolRegistry.h"
#include "KismetAIMutationIntent.h"
#include "KismetAIAuthority.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FString MakeCallHash(const FString& ToolName, const FString& CanonicalArgs, const FString& State)
	{
		return KismetAIMutationIntent::ReplayKey(ToolName, CanonicalArgs) + TEXT("\x1f") + State;
	}

	bool IsMutation(const FString& ToolName, const FString& CanonicalArgs)
	{
		TSharedPtr<FJsonObject> Args;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CanonicalArgs), Args))
			return FKismetAIToolRegistry::IsMutation(ToolName);
		return KismetAIAuthority::DescribeOperation(ToolName, Args).RequiresAuthority();
	}

	bool IsRead(const FString& ToolName, const FString& CanonicalArgs)
	{
		return FKismetAIToolRegistry::IsIdempotentRead(ToolName)
			|| (FKismetAIToolRegistry::IsMutation(ToolName) && !IsMutation(ToolName, CanonicalArgs));
	}
}

bool FKismetAILoopHealth::TextDescribesPendingAction(const FString& Text)
{
	if (Text.IsEmpty()) { return false; }
	const FString L = Text.ToLower();
	static const TCHAR* IntentMarkers[] = {
		TEXT("i'll "), TEXT("i will "), TEXT("let me "), TEXT("let's "),
		TEXT("i'm going to "), TEXT("i am going to "), TEXT("going to add"),
		TEXT("going to create"), TEXT("going to wire"), TEXT("now i'll"),
		TEXT("now i will"), TEXT("next, i"), TEXT("i need to add"),
		TEXT("i need to create"), TEXT("i'll add"), TEXT("i'll create"),
		TEXT("i'll wire"), TEXT("i'll set"),
	};
	for (const TCHAR* Marker : IntentMarkers)
	{
		if (L.Contains(Marker)) { return true; }
	}
	return false;
}

FString FKismetAILoopHealth::GetDuplicateReadMessage()
{
	return TEXT("{\"success\":true,\"duplicate\":true,\"note\":\"DUPLICATE_CALL: you already ran this exact call with identical arguments earlier and got the same result. Don't repeat identical read-only calls — act on what you already know, or change the query/approach.\"}");
}

FString FKismetAILoopHealth::GetRepeatedFailedEditMessage()
{
	return TEXT("<tool_use_error>{\"success\":false,\"error\":{\"error_code\":\"REPEATED_FAILED_EDIT\",\"message\":\"REPEATED_FAILED_EDIT: you already sent this exact edit and it failed the same way; nothing has changed since, so it will fail again. Do NOT resend it. Re-read the graph with read_blueprint(mode=\\\"describe\\\") to get real node/pin names, change your approach, or call ask_user. If the target can't be edited with these tools (e.g. a Material node graph has no edit tool), say so instead of retrying.\"}}</tool_use_error>");
}

EToolCallDisposition FKismetAILoopHealth::DecideDisposition(const FString& ToolName, const FString& CanonicalArgs, const FString& ObservedState)
{
	const FString CallHash = MakeCallHash(ToolName, CanonicalArgs, ObservedState);

	if (IsRead(ToolName, CanonicalArgs))
	{
		// R3: the model already has this answer.
		if (SeenReadOnlyCallHashes.Contains(CallHash))
		{
			return EToolCallDisposition::SkipDuplicateRead;
		}
		return EToolCallDisposition::Execute;
	}

	if (IsMutation(ToolName, CanonicalArgs))
	{
		// A mutation was attempted, which is what R6 gates on — recorded here
		// rather than on success, matching the pre-extraction behavior.
		bAnyMutation = true;

		// R3b: this exact edit already failed and nothing has changed since.
		if (SeenFailedMutationCallHashes.Contains(CallHash))
		{
			return EToolCallDisposition::SkipRepeatedFailedEdit;
		}
	}

	return EToolCallDisposition::Execute;
}

void FKismetAILoopHealth::OnToolResult(const FString& ToolName, const FString& CanonicalArgs,
	bool bShortCircuited, bool bErrored, const FString& ObservedState)
{
	if (bShortCircuited) return;
	const FString Key = MakeCallHash(ToolName, CanonicalArgs, ObservedState);
	if (IsRead(ToolName, CanonicalArgs))
	{
		if (!bErrored) SeenReadOnlyCallHashes.Add(Key);
		return;
	}
	if (!IsMutation(ToolName, CanonicalArgs)) return;
	if (bErrored)
	{
		SeenFailedMutationCallHashes.Add(Key);
		return;
	}
	bTurnMutated = true;
	SeenReadOnlyCallHashes.Reset();
}

FString FKismetAILoopHealth::ObservedStateKey(const FString& ToolName, const TSharedPtr<FJsonObject>& Args,
	const TMap<FString, FString>& Revisions)
{
	TArray<FString> Resources;
	for (const auto& Target : KismetAIAuthority::DescribeOperation(ToolName, Args).Targets)
		Resources.AddUnique(Target.IsResolved() ? Target.PackagePath.ToLower() : Target.Raw);
	if (Args)
	{
		for (const TCHAR* Field : { TEXT("blueprint"), TEXT("widget"), TEXT("material"), TEXT("instance"), TEXT("asset"), TEXT("asset_path") })
		{
			FString Raw, Normalized;
			if (Args->TryGetStringField(Field, Raw))
				Resources.AddUnique(KismetAIAuthority::NormalizeAssetReference(Raw, Normalized) ? Normalized.ToLower() : Raw);
		}
	}
	Resources.Sort();
	TArray<FString> States;
	for (const FString& Resource : Resources)
	{
		FString Revision = TEXT("unobserved");
		const FString* Canonical = Revisions.Find(Resource);
		if (Canonical) Revision = *Canonical;
		for (const auto& Pair : Revisions)
		{
			if (Canonical) break;
			FString Normalized;
			const FString Key = KismetAIAuthority::NormalizeAssetReference(Pair.Key, Normalized) ? Normalized.ToLower() : Pair.Key;
			if (Key == Resource) { Revision = Pair.Value; break; }
		}
		States.Add(Resource + TEXT("=") + Revision);
	}
	return FString::Join(States, TEXT("\n"));
}

void FKismetAILoopHealth::OnTurnEnd(int32 NumToolCalls)
{
	// A turn with no tool calls is not investigation; it does not extend the
	// barren streak.
	if (NumToolCalls > 0)
	{
		ConsecutiveReadOnlyTurns = bTurnMutated ? 0 : (ConsecutiveReadOnlyTurns + 1);
	}
	bTurnMutated = false;
}

FString FKismetAILoopHealth::BuildOrchestratorNote(int32 TurnsRemaining) const
{
	TArray<FString> Notes;

	// R1: hard commit-now on what will be the model's last turn.
	if (TurnsRemaining == 1)
	{
		Notes.Add(TEXT("BUDGET — FINAL TURN NEXT: your next turn is the LAST one you will get. Make any remaining writes/edits and call compile_blueprint to verify NOW. Do not start new investigation."));
	}
	// R2: surface the shrinking budget so the model isn't blind to its deadline.
	else if (TurnsRemaining >= 2 && TurnsRemaining <= 5)
	{
		Notes.Add(FString::Printf(TEXT("BUDGET: %d working turns remain. Prioritize finishing and verifying (compile_blueprint) over further exploration."), TurnsRemaining));
	}

	// R4: stop spinning on read-only investigation.
	if (ConsecutiveReadOnlyTurns >= 3)
	{
		Notes.Add(FString::Printf(TEXT("NO PROGRESS: you have made %d turns of tool calls without changing the Blueprint. Stop investigating — make your best-effort write_blueprint/edit_blueprint now, or call ask_user if a required input is genuinely missing."), ConsecutiveReadOnlyTurns));
	}

	if (Notes.Num() == 0)
	{
		return FString();
	}
	return TEXT("[orchestrator] ") + FString::Join(Notes, TEXT(" "));
}

bool FKismetAILoopHealth::ShouldNudgePrematureEndTurn(const FString& AssistantText, bool bPlanModeActive)
{
	if (bEndTurnNudged || bPlanModeActive || bAnyMutation)
	{
		return false;
	}
	if (!TextDescribesPendingAction(AssistantText))
	{
		return false;
	}
	bEndTurnNudged = true;  // one-shot
	return true;
}
